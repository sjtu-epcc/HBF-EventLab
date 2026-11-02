#include "hbflab/kv.hpp"
#include "ptx_fixture.hpp"
#include <iostream>
using namespace hbflab;
int main(int argc,char** argv)try{
    const std::string name=argc>1?argv[1]:"publish";
    CapacityOptions o;o.logical_pages=8;o.frames=1;o.backend.geometry={1,1,1,4,2,1};
    if(name=="failure")o.backend.fail_program_after=8;
    CapacityStore store(o);KvOptions ko;ko.layers=2;ko.heads=1;ko.head_dimension=1024;ko.element_bytes=2;ko.tokens_per_block=2;ko.max_tokens=4;
    KvCache kv(store,ko);auto seq=kv.create();std::vector<unsigned short> k(3*1024),v(k.size());
    for(unsigned i=0;i<k.size();++i){k[i]=i+1;v[i]=i+5;}
    auto tx=kv.begin_append(seq,3);
    if(name=="failure"){rejects([&]{kv.write_layer(tx,0,k.data(),v.data(),k.size()*2);});require(kv.poisoned(),"partial layer poisons");}
    else if(name=="abort"){kv.abort(tx);require(kv.poisoned(),"explicit abort poisons");rejects([&]{kv.commit(tx);});}
    else{
        rejects([&]{kv.commit(tx);});rejects([&]{kv.locate_layer(tx,0,0,KvKind::Key);});
        rejects([&]{kv.release(seq);});rejects([&]{kv.truncate(seq,0);});rejects([&]{kv.begin_append(seq,1);});
        if(name=="identity"){auto wrong=tx;++wrong.value;rejects([&]{kv.write_layer(wrong,0,k.data(),v.data(),k.size()*2);});wrong=tx;++wrong.cache;rejects([&]{kv.commit(wrong);});}
        kv.write_layer(tx,1,k.data(),v.data(),k.size()*2);require(kv.length(seq)==0,"global length not prematurely published");
        rejects([&]{kv.locate(seq,1,0,KvKind::Key);});rejects([&]{kv.locate_layer(tx,0,0,KvKind::Key);});
        rejects([&]{kv.write_layer(tx,1,k.data(),v.data(),k.size()*2);});
        auto slice=kv.locate_layer(tx,1,2,KvKind::Value);std::vector<unsigned short> result(1024);store.read(slice.allocation,slice.offset,result.data(),2048);
        require(std::equal(result.begin(),result.end(),v.begin()+2048),"pending layer bytes across page/block");
        kv.write_layer(tx,0,k.data(),v.data(),k.size()*2);
        auto pin=store.pin(slice.allocation,0);rejects([&]{kv.commit(tx);});store.unpin(pin);kv.commit(tx);
        require(kv.length(seq)==3,"global publish");rejects([&]{kv.commit(tx);});
        for(unsigned l=0;l<2;++l)for(unsigned t=0;t<3;++t){kv.read(seq,l,t,KvKind::Key,result.data(),2048);require(std::equal(result.begin(),result.end(),k.begin()+t*1024),"committed bytes");}
        if(name=="truncate"){kv.truncate(seq,1);auto next=kv.begin_append(seq,2);kv.write_layer(next,0,v.data(),k.data(),4096);kv.write_layer(next,1,v.data(),k.data(),4096);kv.commit(next);kv.read(seq,0,2,KvKind::Key,result.data(),2048);require(std::equal(result.begin(),result.end(),v.begin()+1024),"truncate append replaces stale tail");}
        rejects([&]{kv.begin_append(seq,2);});kv.release(seq);require(store.free_pages()==8,"released all blocks");
    }
    std::cout<<name<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
